import http from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT || 4173);
const types = {'.html':'text/html','.css':'text/css','.js':'text/javascript','.md':'text/plain'};
http.createServer(async(req,res)=>{
  try {
    const url = new URL(req.url, 'http://localhost');
    const relative = decodeURIComponent(url.pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.resolve(root, relative);
    if (!file.startsWith(root + path.sep)) {res.writeHead(403); res.end(); return;}
    const data = await readFile(file);
    res.writeHead(200, {'Content-Type':types[path.extname(file)]||'application/octet-stream','Cache-Control':'no-cache'});
    res.end(data);
  } catch {res.writeHead(404);res.end('Not found');}
}).listen(port,'127.0.0.1',()=>console.log(`Atmos preview: http://localhost:${port}`));
