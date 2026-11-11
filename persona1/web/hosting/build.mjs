import fs from 'node:fs';
import path from 'node:path';
const types={'.html':'text/html; charset=utf-8','.mjs':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.png':'image/png','.json':'application/json; charset=utf-8','.rgba':'application/octet-stream'};
const assets={};
for(const name of fs.readdirSync('public')){
 const type=types[path.extname(name)];
 if(type)assets[name]={type,data:fs.readFileSync('public/'+name).toString('base64')};
}
fs.mkdirSync('dist/server',{recursive:true});
fs.mkdirSync('dist/.openai',{recursive:true});
fs.writeFileSync('dist/server/index.js','const ASSETS='+JSON.stringify(assets)+';\n'+fs.readFileSync('worker.mjs','utf8'));
fs.copyFileSync('.openai/hosting.json','dist/.openai/hosting.json');
console.log('Worker prepared with '+Object.keys(assets).length+' static assets.');
