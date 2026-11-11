const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, 'dist');
const types = new Map([
    ['.html', 'text/html; charset=utf-8'],
    ['.css', 'text/css; charset=utf-8'],
    ['.png', 'image/png'],
    ['.mjs', 'text/javascript; charset=utf-8'],
    ['.js', 'text/javascript; charset=utf-8'],
    ['.wasm', 'application/wasm'],
    ['.json', 'application/json; charset=utf-8'],
    ['.rgba', 'application/octet-stream'],
    ['.iso', 'application/octet-stream'],
    ['.elf', 'application/octet-stream'],
    ['.csv', 'text/csv; charset=utf-8'],
]);

http.createServer((req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    const pathname = req.url?.split('?')[0];
    const name = pathname === '/' ? 'index.html' : pathname?.slice(1);
    if (!name || !/^[a-zA-Z0-9_.-]+$/.test(name) || !types.has(path.extname(name).toLowerCase())) {
        res.writeHead(404).end();
        return;
    }
    const file = fs.createReadStream(path.join(root, name));
    file.once('open', () => {
        res.setHeader('Content-Type', types.get(path.extname(name).toLowerCase()));
        file.pipe(res);
    });
    file.once('error', () => {
        if (!res.headersSent) res.writeHead(404);
        res.end();
    });
    res.once('close', () => file.destroy());
}).listen(8613, '127.0.0.1', () => console.log('http://127.0.0.1:8613/'));
