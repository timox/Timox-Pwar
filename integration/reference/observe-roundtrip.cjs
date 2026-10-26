// Opt-in real-machine observation. Linux must link pwar-receive to pwar-send.
// Reuses the existing Timox API client and signal probe; edits no engine sources.
const {spawn} = require('node:child_process');
const {createInterface} = require('node:readline');
const fs = require('node:fs');
const path = require('node:path');
const {randomUUID} = require('node:crypto');
const root = path.resolve(__dirname, '../../..');
const {ApiClient} = require(path.join(root, 'gui/electron/api-client'));
const WebSocket = require(path.join(root, 'gui/node_modules/ws'));
const api = new ApiClient({baseUrl:'http://127.0.0.1:52525',
    webSocketUrl:'ws://127.0.0.1:52525/api/v1/ws', WebSocket});
const tag = `pwar-observe-${randomUUID()}`;
const folder = path.join(root, 'build_pwar_reconciled_tests', tag);
const children = [];
let transaction = 0;
function record(name,value) {
    fs.writeFileSync(path.join(folder,name),JSON.stringify(value,null,2));
}
const timeout = (promise, ms) => new Promise((resolve,reject) => {
    const timer=setTimeout(()=>reject(new Error('Observation deadline exceeded')),ms);
    promise.then(v=>{clearTimeout(timer);resolve(v)},e=>{clearTimeout(timer);reject(e)});
});
function start(exe,args,label) {
    const process=spawn(exe,args,{windowsHide:true,stdio:['pipe','pipe','pipe']});
    const lines=[];
    const output=fs.createWriteStream(path.join(folder,`${label}.jsonl`));
    const errors=fs.createWriteStream(path.join(folder,`${label}.stderr`));
    process.stderr.pipe(errors); process.stdout.pipe(output);
    createInterface({input:process.stdout}).on('line',line=>{
        try {lines.push(JSON.parse(line));} catch {}
    });
    const done=new Promise((resolve,reject)=>{process.once('error',reject);process.once('exit',resolve)});
    done.catch(()=>{});
    const child={process,lines,done}; children.push(child); return child;
}
async function ready(child,event) {
    const end=Date.now()+15000;
    while(Date.now()<end) {
        const value=child.lines.find(v=>v.event===event); if(value)return value;
        if(child.process.exitCode!==null)throw new Error(`Child exited before ${event}`);
        await new Promise(r=>setTimeout(r,50));
    }
    throw new Error(`Missing ${event}`);
}
async function clientStatus(child,id) {
    child.process.stdin.write(`${JSON.stringify({version:1,id,command:'status'})}\n`);
    const end=Date.now()+3000;
    while(Date.now()<end) {
        const response=child.lines.find(value=>value.id===id);
        if(response) {
            if(!response.ok||!response.status)throw new Error(`PWAR status request failed: ${id}`);
            return response.status;
        }
        if(child.process.exitCode!==null)throw new Error('PWAR client exited during status request');
        await new Promise(resolve=>setTimeout(resolve,20));
    }
    throw new Error(`Missing PWAR status response: ${id}`);
}
function recordStatus(name,phase,elapsedMs,status) {
    fs.appendFileSync(path.join(folder,'pwar-status.jsonl'),JSON.stringify({
        atUtc:new Date().toISOString(),phase,elapsedMs,requestId:name,status})+'\n');
}
async function apply(state,routes) {
    const payload={physicalDriverId:state.engine.physicalDriverId,
        sampleRate:state.engine.sampleRate,bufferFrames:state.engine.bufferFrames,
        clockSource:state.engine.clockSource,expectedConfigurationRevision:state.configurationRevision,routes};
    const id=++transaction;
    record(`apply-${id}-request.json`,payload);
    try {
        const result=await timeout(api.applyConfiguration(payload),15000);
        record(`apply-${id}-response.json`,{success:true,result});
        return result;
    } catch(e) {
        record(`apply-${id}-response.json`,{success:false,message:e.message,code:e.code,operation:e.operation});
        throw e;
    }
}
async function main() {
    fs.mkdirSync(folder,{recursive:true});
    await timeout(api.connect(),10000);
    const before=await api.getState();
    if(before.engine.state!=='running'||before.engine.sampleRate!==48000||before.engine.clockSource.kind!=='asio')
        throw new Error('Requires running physical ASIO at 48 kHz');
    fs.writeFileSync(path.join(folder,'before.json'),JSON.stringify(before,null,2));
    const client=start(path.join(root,'build_pwar_reconciled_tests/Release/PwarAsioClient.exe'),
        ['--driver-clsid','{A4D39126-78CB-4D89-9E0A-54494D4F5856}','--peer','10.1.0.101','--stdio'],'pwar');
    await ready(client,'asio_start_accepted');
    const probe=role=>{
        const exe=path.join(folder,`PwarSignal-${role}.exe`);
        fs.copyFileSync(path.join(root,'build_codex_110/Release/AsioEngineSignalProbe.exe'),exe);
        return start(exe,['--dll','C:/Program Files/Steinberg/VirtualASIO/1.2.7/TimoxVasio.dll',
            '--role',role,'--seconds',role==='send'?'12':'10','--analysis','bridge'],role);
    };
    const sender=probe('send'), receiver=probe('receive');
    await Promise.all([ready(sender,'ready'),ready(receiver,'ready')]);
    const endpoint=(child,direction)=>`virtual:TimoxVasio:${child.process.pid}:${direction}:1`;
    const state=await api.getState();
    const routes=[{id:tag,sourceEndpointId:endpoint(sender,'output'),destinationEndpointId:endpoint(client,'input'),gainDb:0,mute:false},
        {id:`${tag}-return`,sourceEndpointId:endpoint(client,'output'),destinationEndpointId:endpoint(receiver,'input'),gainDb:0,mute:false}];
    await apply(state,[...state.configuredRoutes,...routes]);
    recordStatus('snapshot-before-signal','before_signal',0,
        await clientStatus(client,'snapshot-before-signal'));
    sender.process.stdin.end('start\n'); receiver.process.stdin.end('start\n');
    const signalStart=Date.now();
    let snapshot=0;
    const snapshots=(async()=>{
        while(sender.process.exitCode===null||receiver.process.exitCode===null) {
            const id=`snapshot-${++snapshot}`;
            recordStatus(id,'signal_window',Date.now()-signalStart,
                await clientStatus(client,id));
            await new Promise(resolve=>setTimeout(resolve,1000));
        }
    })();
    await timeout(Promise.all([sender.done,receiver.done]),35000);
    await snapshots;
    recordStatus('snapshot-after-signal','after_signal',Date.now()-signalStart,
        await clientStatus(client,'snapshot-after-signal'));
    const result=receiver.lines.find(v=>v.event==='result');
    console.log(JSON.stringify({folder,result,physicalDriverId:before.engine.physicalDriverId}));
    if(!result?.passed)process.exitCode=1;
}
main().catch(e=>{console.error(e);process.exitCode=1}).finally(async()=>{
    try {
        const state=await api.getState();
        const routes=state.configuredRoutes.filter(r=>r.id!==tag&&r.id!==`${tag}-return`);
        if(routes.length!==state.configuredRoutes.length)await apply(state,routes);
        const after=await api.getState();
        record('after-cleanup.json',after);
        if(after.configuredRoutes.some(r=>r.id===tag||r.id===`${tag}-return`))
            throw new Error('Temporary routes remain after cleanup');
    } catch(e) {console.error('ROUTE CLEANUP FAILED',tag,e);process.exitCode=1;}
    for(const child of children) {
        child.process.stdin.end();
        try {await timeout(child.done,3000);}catch{child.process.kill();}
    }
    api.close();
});
