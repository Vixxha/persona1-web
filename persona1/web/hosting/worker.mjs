const runtimeFiles = new Map([
 ['Persona.iso','application/octet-stream'],
 ['persona.wasm','application/wasm'],
 ['EBOOT_DECRYPTED.ELF','application/octet-stream'],
 ['nids_persona1.csv','text/csv; charset=utf-8'],
]);
const runtimeOrigin = 'https://github.com/Vixxha/persona1-web/releases/download/web-v1/';
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
export default {
 async fetch(request) {
  const url = new URL(request.url);
  if (!['GET','HEAD'].includes(request.method)) return new Response('Method not allowed',{status:405,headers:{Allow:'GET, HEAD'}});
  if (url.pathname === '/favicon.ico') return Response.redirect(new URL('/favicon.png',url),302);
  const name = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
  if (!/^[a-zA-Z0-9_.-]+$/.test(name)) return new Response('Not found',{status:404});
  if (runtimeFiles.has(name)) {
   const upstreamHeaders = new Headers({'User-Agent':'Persona1-Web','Accept-Encoding':'identity'});
   if (request.headers.has('Range')) upstreamHeaders.set('Range',request.headers.get('Range'));
   let upstream;
   try { upstream = await fetch(runtimeOrigin + name,{method:request.method,headers:upstreamHeaders,redirect:'follow'}); }
   catch { return new Response('No se pudo descargar el archivo del juego. Intenta de nuevo.',{status:502,headers:headers('text/plain; charset=utf-8')}); }
   if (!upstream.ok) return new Response('El archivo del juego no esta disponible temporalmente.',{status:502,headers:headers('text/plain; charset=utf-8')});
   const h = headers(runtimeFiles.get(name));
   h.set('Cache-Control','public, max-age=31536000, immutable');
   for (const key of ['Content-Length','Content-Range','Accept-Ranges','ETag','Last-Modified']) {
    if(upstream.headers.has(key))h.set(key,upstream.headers.get(key));
   }
   return new Response(request.method === 'HEAD' ? null : upstream.body,{status:upstream.status,headers:h});
  }
  const asset = ASSETS[name];
  if (!asset) return new Response('Not found',{status:404});
  const h = headers(asset.type);
  h.set('Cache-Control', name === 'index.html' || name === 'state-build.json' ? 'no-cache' : 'public, max-age=3600');
  if(!decoded.has(name)){
   const text=atob(asset.data), bytes=new Uint8Array(text.length);
   for(let i=0;i<text.length;++i)bytes[i]=text.charCodeAt(i);
   decoded.set(name,bytes);
  }
  const bytes=decoded.get(name);
  h.set('Content-Length',String(bytes.length));
  return new Response(request.method === 'HEAD' ? null : bytes,{headers:h});
 }
};
