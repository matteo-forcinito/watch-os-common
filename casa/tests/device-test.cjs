const fs=require('fs'),path=require('path'),assert=require('assert/strict');
if(!process.env.CASA_QA_PAIRING_FILE||!process.env.CASA_QA_HA_URL)throw Error('Set CASA_QA_PAIRING_FILE and CASA_QA_HA_URL; see README.md');
const auth=JSON.parse(fs.readFileSync(process.env.CASA_QA_PAIRING_FILE)),haUrl=process.env.CASA_QA_HA_URL;
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function request(route,method='GET',body,authorized=true){
  const response=await fetch(new URL(route,auth.url),{method,headers:{...(authorized?{'X-Key':auth.token}:{}),'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(20000)});
  const text=await response.text();return {status:response.status,text,json:()=>JSON.parse(text)};
}
async function waitFor(predicate,seconds=45){await pause(1000);for(let i=0;i<seconds;i++){const state=(await request('/casa/api/state')).json();if(predicate(state))return state;await pause(1000);}throw Error('Device state did not converge');}
(async()=>{
  await request('/casa/');await pause(2000);
  const initial=(await request('/casa/api/config')).json();
  assert.equal(initial.has_token,false,'Refusing to overwrite existing HA credentials');
  const fixtures=await(await fetch('http://127.0.0.1:8769/qa')).json();
  const config={...fixtures.lastSave,url:haUrl,token:'casa-qa-token'};
  let configured=false;
  try{
    assert.equal((await request('/casa/api/state','GET',undefined,false)).status,401);
    assert.equal((await request('/casa/api/config','POST',config)).json().ok,true);configured=true;
    let state=await waitFor(state=>state.connected&&state.entities.filter(e=>e.known).length===10);
    assert.equal(state.entities.find(e=>e.id==='sensor.living_temperature').state,'21.4');
    assert.equal(state.entities.find(e=>e.id==='switch.desk_plug').category,'Prese');
    assert.equal(state.entities.find(e=>e.id==='sensor.offline').available,false);
    assert.equal((await request('/casa/api/config','POST',{...config,token:''})).json().ok,true,'Repeated save must work on FAT');
    await waitFor(state=>state.connected&&state.entities.filter(e=>e.known).length===10);
    const readback=await request('/casa/api/config');assert(!readback.text.includes('casa-qa-token'));
    assert.equal((await request('/casa/api/config','POST',{...config,favorites:[config.favorites[0],config.favorites[0]]})).json().ok,false);
    assert.equal((await request('/casa/api/config','POST',{...config,url:'http://other.invalid',token:''})).json().ok,false);
    for(const action of [{id:'sensor.living_temperature',service:'turn_on'},{id:'light.missing',service:'turn_on'},{id:'light.kitchen',service:'turn_on',data:{entity_id:'light.other'}}])assert.equal((await request('/casa/api/action','POST',action)).json().ok,false);
    const previous=(await waitFor(s=>s.connected)).entities.find(e=>e.id==='light.kitchen').state;
    assert.equal((await request('/casa/api/action','POST',{id:'light.kitchen',service:'toggle'})).json().ok,true);
    await waitFor(s=>s.entities.find(e=>e.id==='light.kitchen').state!==previous);
    assert.equal((await request('/casa/api/action','POST',{id:'climate.living',service:'set_temperature',data:{temperature:23.5}})).json().ok,true);
    await waitFor(s=>s.entities.find(e=>e.id==='climate.living').target_temp===23.5);
    assert.equal((await request('/casa/api/action','POST',{id:'climate.living',service:'set_temperature',data:{temperature:999}})).json().ok,true);
    await waitFor(s=>s.message.startsWith('Home Assistant:'));
    assert.equal((await request('/casa/api/action','POST',{id:'climate.living',service:'set_temperature',data:{temperature:23.5}})).json().ok,true);
    await waitFor(s=>s.connected&&!s.message.startsWith('Home Assistant:'));
    await fetch('http://127.0.0.1:8769/qa',{method:'POST',body:JSON.stringify({drop:true})});
    await waitFor(s=>s.connected&&s.entities.filter(e=>e.known).length===10);
    assert.equal((await request('/casa/api/config','POST',{...config,token:'invalid-qa-token'})).json().ok,true);
    await waitFor(s=>!s.connected&&s.message.startsWith('Token'));
    assert.equal((await request('/casa/api/action','POST',{id:'light.kitchen',service:'toggle'})).json().ok,false);
    assert.equal((await request('/casa/api/config','POST',config)).json().ok,true);
    await waitFor(s=>s.connected&&s.entities.filter(e=>e.known).length===10);
    console.log('CASA_RECONNECT_AUTH_AND_SERVICE_ERRORS_PASS');
    const expanded={...config,favorites:[...config.favorites,...Array.from({length:246},(_,i)=>({id:'sensor.casa_qa_'+i,name:'Sensore collaudo '+i,area:'Collaudo '+(i%20),device_class:'temperature',dashboards:['Collaudo / Sensori']}))]};
    assert.equal((await request('/casa/api/config','POST',expanded)).json().ok,true);
    await waitFor(s=>s.connected&&s.entities.length===256&&s.entities.filter(e=>e.known).length===10);
    console.log('CASA_256_ENTITIES_PASS',JSON.stringify((await request('/api/system')).json().memory));
    assert.equal((await request('/casa/api/config','POST',config)).json().ok,true);
    await waitFor(s=>s.connected&&s.entities.length===10&&s.entities.filter(e=>e.known).length===10);
    console.log('CASA_DEVICE_QA_PASS: authenticated routes, HA websocket auth/subscription, live deltas, light and thermostat commands, FAT resave, invalid requests, token redaction');
    console.log('MEMORY_CASA_ACTIVE',JSON.stringify((await request('/api/system')).json().memory));
  }catch(error){console.error('QA_FAILURE',error);throw error;}finally{
    if(configured){let cleared=false;for(let i=0;i<5&&!cleared;i++){cleared=(await request('/casa/api/disconnect','POST',{})).json().ok;if(!cleared)await pause(1000);}assert(cleared);await waitFor(s=>!s.configured&&s.entities.length===0);console.log('CASA_QA_CONFIGURATION_REMOVED');}
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
