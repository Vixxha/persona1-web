import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
const {default:worker}=await import('./dist/server/index.js');
const req=(p,options)=>new Request('https://example.test'+p,options);
let r=await worker.fetch(req('/'));
assert.equal(r.status,200);
assert.equal(r.headers.get('Cross-Origin-Opener-Policy'),'same-origin');
assert.equal(r.headers.get('Cross-Origin-Embedder-Policy'),'require-corp');
assert.ok((await r.text()).includes('id="start"'));
r=await worker.fetch(req('/favicon.png'));
const hash=b=>createHash('sha256').update(b).digest('hex');
assert.equal(hash(Buffer.from(await r.arrayBuffer())),hash(fs.readFileSync('public/favicon.png')));
const env={PERSONA_UPLOAD_SECRET:'even-an-old-secret-cannot-enable-writes',BUCKET:{
 async get(name,options){
  assert.equal(name,'Persona.iso');assert.equal(options.range.get('Range'),'bytes=0-2');
  return {body:new ReadableStream({start(c){c.enqueue(new Uint8Array([1,2,3]));c.close()}}),size:820740096,range:{offset:0,length:3},httpEtag:'"test"'};
 },
 async head(){return{size:820740096,range:{offset:0,length:820740096},httpEtag:'"test"'}},
}};
assert.equal((await worker.fetch(req('/../secret'),env)).status,404);
assert.equal((await worker.fetch(req('/Persona.iso',{method:'POST'}),env)).status,405);
assert.equal((await worker.fetch(req('/_upload/Persona.iso',{method:'POST'}),env)).status,405);
assert.equal((await worker.fetch(req('/_upload/Persona.iso'),env)).status,404);
const originalFetch=globalThis.fetch;
globalThis.fetch=()=>{throw Error('Runtime must not depend on another host')};
r=await worker.fetch(req('/Persona.iso',{headers:{Range:'bytes=0-2'}}),env);
assert.equal(r.status,206);assert.equal(r.headers.get('Content-Range'),'bytes 0-2/820740096');
assert.deepEqual([...new Uint8Array(await r.arrayBuffer())],[1,2,3]);
r=await worker.fetch(req('/Persona.iso',{method:'HEAD'}),env);
assert.equal(r.status,200);assert.equal(r.headers.get('Content-Length'),'820740096');assert.equal((await r.arrayBuffer()).byteLength,0);
globalThis.fetch=originalFetch;
console.log('PASS: independent storage, no outbound fetches, ranged reads, HEAD and no public writes.');
