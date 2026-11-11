/* 浏览器只负责显示和发送操作意图。SDK 报文、锁、保存和预览均由 C++ 处理。 */
'use strict';
const $ = id => document.getElementById(id);
let token = '', state = {}, busy = false, polling = false, previewPaused = false;
let hold = null, toastTimer, retryTimer, sessionReady = false;
let deviceSettings = {};
const names = {visible:'可见光',thermal:'热成像',pip:'拼接画面',other:'相机自定义画面'};
const recordNames = {stopped:'未录像',recording:'录像中',no_sd_card:'未插卡',recording_card_error:'写卡异常'};
function log(text, error=false) {
  const row = document.createElement('p'); row.className = 'log-entry' + (error?' error':'');
  const time = document.createElement('span'); time.className='log-time'; time.textContent=new Date().toLocaleTimeString('zh-CN',{hour12:false});
  row.append(time,document.createTextNode(text)); $('activity').prepend(row);
  while ($('activity').children.length>30) $('activity').lastChild.remove();
}
function toast(text,error=false) {
  clearTimeout(toastTimer); $('toast').textContent=text; $('toast').className='toast'+(error?' error':'');
  toastTimer=setTimeout(()=>$('toast').classList.add('hidden'),4500);
}
function updateDeviceSettings(name,result){
  if(name==='resolution'){
    deviceSettings.resolution=result;
    $('encodingCurrent').textContent=`当前 ${result.width} × ${result.height} · ${result.codec.toUpperCase()} · ${result.bitrate_kbps} Kbps · ${result.fps} fps`;
    const size=`${result.width}x${result.height}`;
    $('cameraResolution').querySelectorAll('[data-record-only]').forEach(option=>{option.hidden=result.stream!=='record';option.disabled=result.stream!=='record';});
    $('cameraResolution').querySelectorAll('[data-current]').forEach(option=>option.remove());
    if(![...$('cameraResolution').options].some(option=>option.value===size)){
      const option=document.createElement('option');option.value=size;option.textContent=`当前原生 ${result.width} × ${result.height}（仅查询）`;option.disabled=true;option.dataset.current='true';$('cameraResolution').append(option);
    }
    $('cameraResolution').value=size;$('cameraCodec').value=result.codec;$('cameraBitrate').value=result.bitrate_kbps;
  }
  if(name==='gimbal_mode')$('gimbalMode').value=result.mode;
  if(name==='zoom'){
    $('zoomRatio').value=result.ratio;$('zoomRatio').max=result.max_ratio;
    $('zoomInfo').textContent=`当前 ${result.ratio}×，相机回报最大 ${result.max_ratio}×（数字变倍）`;
  }
  if(name==='zoom_link')$('zoomLink').value=result.linked?'on':'off';
  if(name==='thermal_output'){deviceSettings.thermal=result;$('thermalOutput').value=result.mode;}
  if(name==='thermal_frame')$('thermalFrameInfo').textContent=result.message;
}
async function request(url, options={}, timeout=30000) {
  const controller=new AbortController(), timer=setTimeout(()=>controller.abort(),timeout);
  try {const response=await fetch(url,{...options,signal:controller.signal,cache:'no-store'});const data=await response.json();if(!response.ok)throw new Error(data.error||`HTTP ${response.status}`);return data;}
  catch(error){if(error.name==='AbortError')throw new Error('连接超时，请查询状态后再决定是否重试');throw error;}
  finally{clearTimeout(timer);}
}
// 写操作只发送一次。出错时查询状态，不在浏览器自动重发拍照/录像命令。
async function action(actionName, data={}, quiet=false) {
  if(!sessionReady)throw new Error('页面尚未连接，请刷新');
  const isMotion=['move','stop'].includes(actionName);
  if(!isMotion&&busy)return null;
  if(!isMotion){busy=true;render();}
  try {
    const response=await request('/api/action',{method:'POST',headers:{'Content-Type':'application/json','X-ZT6-Token':token},body:JSON.stringify({action:actionName,storage:$('storage').value,...data})},actionName==='photo'&&data.count>1?data.count*65000+5000:30000);
    const result=response.result;
    if(['angle','center'].includes(actionName)&&(result.success!==true||result.status!=='completed'))throw new Error('动作已发送，但未获得到位确认；请查询姿态并确认已重启新版 Web 服务');
    updateDeviceSettings(actionName,result);
    if(!quiet){const descriptions={photo:result.completed_count?`连拍完成（${result.completed_count} 张）`:'拍照完成',record_start:'录像已开始',record_stop:'录像已停止',storage:'保存位置已切换',center:'回中已完成',angle:'目标角度动作已完成',view:'画面来源已切换',auto_record:'自动录像设置已读取/更新',stop:'停止指令已发送',resolution:result.applied?'相机编码参数已设置并确认':'相机编码参数已读取',gimbal_mode:result.changed?'云台模式已设置并确认':'云台模式已读取',zoom:result.changed?'倍率已设置并确认':'倍率已读取',zoom_link:'热像联动设置已读取/确认',thermal_output:result.changed?'热像输出模式已设置并确认':'热像输出模式已读取',thermal_frame:'温度帧请求已接受，未接收温度矩阵'};const text=descriptions[actionName]||'操作完成';log(text+(result.path?' · '+result.path.split('/').pop():''));toast(text);}
    return result;
  } catch(error) {if(!quiet){log(error.message,true);toast(error.message,true);}throw error;}
  finally {if(!isMotion){busy=false;await poll();render();}}
}
function render() {
  const online=!!state.connected, camera=state.camera||{}, onboard=state.onboard||{};
  $('connection').textContent=online?(busy||state.busy?'设备在线 · 操作中':'● 设备在线'):'设备离线';
  $('connection').className='badge '+(online?'online':'offline');
  if(state.selected_storage&&!busy)$('storage').value=state.selected_storage;
  const ob=!!onboard.active, sd=[1,3].includes(camera.record_state), recording=ob||sd;
  $('onboardRecord').textContent=ob?'录像中':'未录像';$('onboardRecord').className=ob?'recording':'';
  $('sdRecord').textContent=recordNames[camera.record_status]||'状态未知';$('sdRecord').className=sd?'recording':'';
  $('stopOnboard').classList.toggle('hidden',!ob);$('stopSD').classList.toggle('hidden',!sd);
  $('recordIndicator').classList.toggle('hidden',!recording);
  document.querySelectorAll('[data-device]').forEach(button=>button.disabled=!online||busy);
  document.querySelectorAll('[data-source]').forEach(button=>{button.disabled=!online||busy||recording;button.classList.toggle('active',button.dataset.source===state.video?.view);});
  $('storage').disabled=busy||recording;
  $('photoCount').disabled=!online||busy;
  $('recordStart').disabled=!online||busy||($('storage').value==='onboard'?ob:sd);
  $('recordStop').disabled=busy||!($('storage').value==='onboard'?ob:sd);
  $('stopMotion').disabled=!sessionReady;
  document.querySelectorAll('[data-setting]').forEach(input=>input.disabled=!online||busy);
  $('cameraCodec').disabled=!online||busy||$('encodingStream').value==='record';
  $('applyResolution').disabled=!online||busy||recording||!(['1920x1080','1280x720'].includes($('cameraResolution').value)||($('encodingStream').value==='record'&&['2560x1440','3840x2160'].includes($('cameraResolution').value)));
  $('applyThermalOutput').disabled=!online||busy||recording||!$('thermalOutput').value;
  const zoomEnabled=state.zoom_enabled===true;
  ['zoomRatio','zoomLink','applyZoom','applyZoomLink'].forEach(id=>$(id).disabled=!zoomEnabled||!online||busy);
  $('applyZoomLink').disabled=!zoomEnabled||!online||busy||!$('zoomLink').value;
  if(!zoomEnabled)$('zoomInfo').textContent='变倍与热像联动暂未启用。';
  $('requestTempFrame').disabled=!online||busy;
  $('autoRecord').disabled=!online||busy;$('readAuto').disabled=!online||busy;
  ['yaw','pitch','roll'].forEach(key=>{$(key).textContent=Number.isFinite(state.attitude?.[key])?state.attitude[key].toFixed(1)+'°':'—';});
  $('mounting').textContent=camera.mounting_direction===2?'倒装 / SDK 坐标':camera.mounting_direction===1?'正装':'—';
  $('viewLabel').textContent=names[state.video?.view]||'相机主码流';
  const preview=state.preview||{}, fresh=preview.age_seconds!==null&&preview.age_seconds<3;
  $('previewInfo').textContent=previewPaused?'预览已暂停':fresh?`MJPEG · ≤ ${preview.fps_limit} FPS · ${preview.width_limit}px`:'等待相机视频';
  $('frameStatus').textContent=previewPaused?'PAUSED':fresh?'● LIVE':'CONNECTING';
  $('storageHint').textContent=$('storage').value==='onboard'?`机载保存：${state.output_dir||'/home/cat/zt6_camera_data'}；照片从当前视频流截帧。`:'云台 SD 保存：原生拍照/卡录；完成后可用文件列表核实是否写入。';
  $('footerStatus').textContent=state.version?`${state.version} · ${state.camera_host}`:'等待 C++ 服务';
}
async function poll(){if(polling)return;polling=true;try{state=await request('/api/status',{},9000);render();}catch(error){state.connected=false;render();}finally{polling=false;}}
function connectVideo(){clearTimeout(retryTimer);if(previewPaused)return;$('liveImage').src='/stream.mjpg?t='+Date.now();}
$('liveImage').addEventListener('load',()=>$('videoPlaceholder').classList.add('hidden'));
$('liveImage').addEventListener('error',()=>{if(!previewPaused){$('videoPlaceholder').classList.remove('hidden');retryTimer=setTimeout(connectVideo,2500);}});
$('previewToggle').onclick=()=>{previewPaused=!previewPaused;$('previewToggle').textContent=previewPaused?'恢复预览':'暂停预览';if(previewPaused){clearTimeout(retryTimer);$('liveImage').removeAttribute('src');$('videoPlaceholder').classList.remove('hidden');}else connectVideo();render();};
$('photo').onclick=()=>{
  const count=Number($('photoCount').value);
  if(!Number.isInteger(count)||count<1||count>100){toast('拍照张数必须是 1–100 的整数',true);return;}
  action('photo',{count}).catch(()=>{});
};
$('recordStart').onclick=()=>action('record_start').catch(()=>{});
$('recordStop').onclick=()=>action('record_stop').catch(()=>{});
$('stopOnboard').onclick=()=>action('record_stop',{storage:'onboard'}).catch(()=>{});
$('stopSD').onclick=()=>action('record_stop',{storage:'sd'}).catch(()=>{});
$('center').onclick=()=>action('center').catch(()=>{});
$('storage').onchange=()=>action('storage').catch(()=>{});
document.querySelectorAll('[data-source]').forEach(button=>button.onclick=async()=>{try{await action('view',{source:button.dataset.source});connectVideo();}catch(error){}});
$('angleForm').onsubmit=event=>{event.preventDefault();action('angle',{yaw:Number($('targetYaw').value),pitch:Number($('targetPitch').value)}).catch(()=>{});};
$('autoRecord').onchange=async()=>{const value=$('autoRecord').value;if(!value)return;try{const result=await action('auto_record',{value});$('autoRecord').value=result.auto_record?'on':'off';}catch(error){$('autoRecord').value='';}};
$('readAuto').onclick=async()=>{try{const result=await action('auto_record',{value:'status'});$('autoRecord').value=result.auto_record?'on':'off';}catch(error){}};
$('readSettings').onclick=async()=>{
  for(const [name,data] of [['resolution',{stream:$('encodingStream').value}],['gimbal_mode',{}],['zoom',{}],['zoom_link',{}],['thermal_output',{}]]){
    if(['zoom','zoom_link'].includes(name)&&state.zoom_enabled!==true)continue;
    try{await action(name,data);}catch(error){}
  }
};
$('encodingStream').onchange=()=>action('resolution',{stream:$('encodingStream').value}).catch(()=>{});
$('cameraResolution').onchange=render;$('thermalOutput').onchange=render;$('zoomLink').onchange=render;
$('resolutionForm').onsubmit=async event=>{
  event.preventDefault();const [width,height]=$('cameraResolution').value.split('x').map(Number);
  const data={stream:$('encodingStream').value,width,height,reapply:true};
  if($('cameraCodec').value)data.codec=$('cameraCodec').value;
  if($('cameraBitrate').value)data.bitrate_kbps=Number($('cameraBitrate').value);
  try{const result=await action('resolution',data);if(result?.applied)connectVideo();}catch(error){}
};
$('modeForm').onsubmit=event=>{event.preventDefault();action('gimbal_mode',{mode:$('gimbalMode').value}).catch(()=>{});};
$('zoomForm').onsubmit=event=>{event.preventDefault();action('zoom',{ratio:Number($('zoomRatio').value)}).catch(()=>{});};
$('applyZoomLink').onclick=()=>action('zoom_link',{value:$('zoomLink').value}).catch(()=>{});
$('applyThermalOutput').onclick=async()=>{try{const result=await action('thermal_output',{mode:$('thermalOutput').value});if(result?.changed)connectVideo();}catch(error){}};
$('requestTempFrame').onclick=()=>action('thermal_frame').catch(()=>{});

// 每次方向请求仅执行 0.18 秒；下一次必须等上一脉冲结束，断网/关页不会留下持续速度请求。
async function beginHold(button){
  if(hold||busy||button.disabled)return;const current={button,active:true};hold=current;button.classList.add('pressed');
  while(current.active){try{await action('move',{yaw:Number(button.dataset.yaw),pitch:Number(button.dataset.pitch)},true);}catch(error){current.active=false;log(error.message,true);break;}if(current.active)await new Promise(resolve=>setTimeout(resolve,90));}
  button.classList.remove('pressed');if(hold===current)hold=null;
}
function releaseHold(sendStop=true){if(hold){hold.active=false;hold.button.classList.remove('pressed');}if(sessionReady&&sendStop)action('stop',{},true).catch(()=>{});}
document.querySelectorAll('[data-yaw]').forEach(button=>{button.addEventListener('pointerdown',event=>{event.preventDefault();button.setPointerCapture(event.pointerId);beginHold(button);});button.addEventListener('pointerup',releaseHold);button.addEventListener('pointercancel',releaseHold);button.addEventListener('lostpointercapture',()=>{if(hold?.button===button)releaseHold();});});
$('stopMotion').onclick=()=>{releaseHold(false);action('stop').catch(()=>{});};
window.addEventListener('blur',()=>{if(hold)releaseHold();});document.addEventListener('visibilitychange',()=>{if(document.hidden&&hold)releaseHold();});
const keys={ArrowUp:'.up',ArrowDown:'.down',ArrowLeft:'.left',ArrowRight:'.right'};
document.addEventListener('keydown',event=>{if(['INPUT','SELECT','TEXTAREA'].includes(event.target.tagName))return;if(keys[event.key]){event.preventDefault();if(!event.repeat)beginHold(document.querySelector(keys[event.key]));}if(event.code==='Space'){event.preventDefault();releaseHold();}if(event.key==='Home'&&!event.repeat){event.preventDefault();action('center').catch(()=>{});}});
document.addEventListener('keyup',event=>{if(keys[event.key]&&hold){event.preventDefault();releaseHold();}});
async function files(){
  $('refreshFiles').disabled=true;
  try {
    const storage=$('storage').value;
    const result=await request(`/api/files?storage=${storage}&kind=${$('fileKind').value}`);
    const list=result.files||result.data?.list||[];
    $('files').replaceChildren();
    if(!list.length){
      const empty=document.createElement('p');empty.className='hint';
      empty.textContent='当前类型暂无文件。';$('files').append(empty);
    }
    list.forEach(item=>{
      const row=document.createElement('div');row.className='file-row';
      const label=document.createElement('span');label.className='file-name';
      const name=item.name||item.path.split('/').pop();
      label.textContent=(item.directory?item.directory+'/':'')+name;row.append(label);
      const link=document.createElement('a');link.textContent='下载';
      // SD 地址交给同源 C++ 服务校验并流式转发，浏览器不直接访问相机内网。
      link.href=storage==='sd'
        ? '/api/download?storage=sd&url='+encodeURIComponent(item.url)
        : '/api/download?storage=onboard&file='+encodeURIComponent(name);
      link.setAttribute('download',name);row.append(link);$('files').append(row);
    });
    log(`已读取 ${list.length} 个${$('fileKind').value==='photo'?'照片':'录像'}文件`);
  } catch(error){log(error.message,true);toast(error.message,true);}
  finally{$('refreshFiles').disabled=false;}
}
$('refreshFiles').onclick=files;$('fileKind').onchange=files;$('clearLog').onclick=()=>$('activity').replaceChildren();
async function init(){render();try{const session=await request('/api/session',{},5000);token=session.token;sessionReady=true;log('C++ 控制服务已连接');await poll();connectVideo();setInterval(poll,1300);}catch(error){log(error.message,true);toast('服务未连接，请检查 SSH 隧道并刷新页面',true);}}
init();
