const states = [
  {entity_id:'sensor.living_temperature',state:'21.4',attributes:{friendly_name:'Temperatura soggiorno',device_class:'temperature',unit_of_measurement:'°C'}},
  {entity_id:'sensor.humidity',state:'56',attributes:{friendly_name:'Umidità',device_class:'humidity',unit_of_measurement:'%'}},
  {entity_id:'light.kitchen',state:'off',attributes:{friendly_name:'Luce cucina',brightness:128,supported_color_modes:['brightness']}},
  {entity_id:'light.desk',state:'on',attributes:{friendly_name:'Luce scrivania',brightness:200,supported_color_modes:['brightness']}},
  {entity_id:'switch.desk_plug',state:'on',attributes:{friendly_name:'Presa scrivania',device_class:'outlet'}},
  {entity_id:'climate.living',state:'heat',attributes:{friendly_name:'Termostato',current_temperature:21.4,temperature:22,min_temp:7,max_temp:30,target_temp_step:.5,hvac_modes:['off','heat','auto']}},
  {entity_id:'sensor.offline',state:'unavailable',attributes:{friendly_name:'Batteria ambiente',device_class:'battery',unit_of_measurement:'%'}},
  {entity_id:'binary_sensor.motion',state:'unavailable',attributes:{friendly_name:'Movimento'}},
  {entity_id:'scene.evening',state:'scening',attributes:{friendly_name:'Sera'}},
  {entity_id:'light.'+'x'.repeat(249),state:'off',attributes:{friendly_name:'<img src=x onerror=window.injected=true>'}},
];
const areas=[{area_id:'living',name:'Soggiorno'},{area_id:'kitchen',name:'Cucina'}];
const devices=[{id:'climate-device',area_id:'living',name:'Termostato soggiorno'},{id:'kitchen-device',area_id:'living',name:'Luce cucina'},{id:'environment-device',area_id:'living',name_by_user:'Sensore soggiorno'},{id:'desk-device',area_id:'living',name:'Scrivania'},{id:'motion-device',area_id:'living',name:'Movimento'}];
const registry=states.map((state,index)=>({entity_id:state.entity_id,area_id:index===2?'kitchen':null,device_id:[0,1,6].includes(index)?'environment-device':index===2?'kitchen-device':index===5?'climate-device':index===7?'motion-device':[3,4].includes(index)?'desk-device':null}));
const dashboards=[{title:'Piano terra',url_path:'piano-terra'}];
const configs={
  default:{views:[{title:'Casa',cards:[{type:'entities',entities:['sensor.living_temperature','switch.desk_plug']},{type:'horizontal-stack',cards:[{entity:'climate.living'}]}]}]},
  'piano-terra':{views:[{title:'Cucina',sections:[{cards:[{type:'area',area:'kitchen'}]}]},{title:'Comfort',cards:[{entity:'sensor.humidity'},{entity:'climate.living'}]}]},
};
function reply(message){
  switch(message.type){
    case 'get_states':return states;
    case 'config/area_registry/list':return areas;
    case 'config/entity_registry/list':return registry;
    case 'config/entity_registry/get_entries':return Object.fromEntries(message.entity_ids.map(id=>[id,registry.find(entity=>entity.entity_id===id)||null]));
    case 'config/device_registry/list':return devices;
    case 'lovelace/dashboards/list':return dashboards;
    case 'lovelace/config':return configs[message.url_path||'default'];
    default:throw new Error('Unsupported fixture command: '+message.type);
  }
}
module.exports={states,areas,registry,devices,dashboards,configs,reply};
