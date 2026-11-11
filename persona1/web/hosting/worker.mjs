const runtimeFiles = new Map([
 ['Persona.iso','application/octet-stream'],
 ['persona.wasm','application/wasm'],
 ['EBOOT_DECRYPTED.ELF','application/octet-stream'],
 ['nids_persona1.csv','text/csv; charset=utf-8'],
]);
const decoded = new Map();
function headers(type) {
 return new Headers({
  'Content-Type':type,
  'Cross-Origin-Opener-Policy':'same-origin',
  'Cross-Origin-Embedder-Policy':'require-corp',
  'Cross-Origin-Resource-Policy':'same-origin',
  'X-Content-Type-Options':'nosniff',
 });
}
async function runtime(request,name,env) {
 if(!env.BUCKET)return new Response('Almacenamiento no disponible.',{status:503,headers:headers('text/plain; charset=utf-8')});
 let item;
 try{
  item=request.method==='HEAD'?await env.BUCKET.head(name):await env.BUCKET.get(name,request.headers.has('Range')?{range:request.headers}:undefined);
 }catch{return new Response('Rango no valido.',{status:416});}
 if(!item)return new Response('El juego se esta preparando. Intenta de nuevo en unos minutos.',{status:503,headers:headers('text/plain; charset=utf-8')});
 const h=headers(runtimeFiles.get(name));
 h.set('Cache-Control','public, max-age=3600');
 h.set('Accept-Ranges','bytes');
 h.set('ETag',item.httpEtag);
 let status=200;
 if(request.method !== 'HEAD' && request.headers.has('Range') && item.range){
  const offset=item.range.offset ?? Math.max(0,item.size-item.range.suffix);
  const length=item.range.length ?? Math.min(item.range.suffix,item.size);
  h.set('Content-Range','bytes '+offset+'-'+(offset+length-1)+'/'+item.size);
  h.set('Content-Length',String(length));status=206;
 }else h.set('Content-Length',String(item.size));
 return new Response(request.method==='HEAD'?null:item.body,{status,headers:h});
}
export default {
 async fetch(request,env={}) {
  const url=new URL(request.url);
  if(!['GET','HEAD'].includes(request.method))return new Response('Method not allowed',{status:405,headers:{Allow:'GET, HEAD'}});
  if(url.pathname==='/favicon.ico')return Response.redirect(new URL('/favicon.png',url),302);
  const name=url.pathname==='/'?'index.html':url.pathname.slice(1);
  if(!/^[a-zA-Z0-9_.-]+$/.test(name))return new Response('Not found',{status:404});
  if(runtimeFiles.has(name))return runtime(request,name,env);
  const asset=ASSETS[name];
  if(!asset)return new Response('Not found',{status:404});
  const h=headers(asset.type);
  h.set('Cache-Control',name==='index.html'||name==='state-build.json'?'no-cache':'public, max-age=3600');
  if(!decoded.has(name)){
   const text=atob(asset.data),bytes=new Uint8Array(text.length);
   for(let i=0;i<text.length;++i)bytes[i]=text.charCodeAt(i);
   decoded.set(name,bytes);
  }
  const bytes=decoded.get(name);h.set('Content-Length',String(bytes.length));
  return new Response(request.method==='HEAD'?null:bytes,{headers:h});
 }
};
