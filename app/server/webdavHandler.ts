import fs from 'node:fs';
import path from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { engine } from './engineState.js';
import { getMimeType, resolveSafePath } from './vfsService.js';

function escapeXml(str: string): string {
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&apos;');
}

function encodeDavHref(virtualPath: string, isDir: boolean): string {
  const segments = virtualPath
    .split('/')
    .filter(Boolean)
    .map((s) => encodeURIComponent(s));
  const base = segments.length === 0 ? '/dav/' : '/dav/' + segments.join('/');
  return isDir && !base.endsWith('/') ? base + '/' : base;
}

function buildPropResponseXml(
  virtualPath: string,
  fullPath: string,
  displayNameOverride?: string
): string {
  const stat = fs.statSync(fullPath);
  const isDir = stat.isDirectory();
  const href = encodeDavHref(virtualPath, isDir);
  const displayName =
    displayNameOverride ?? (virtualPath === '/' ? 'ChainIt Sibling (Z:)' : path.basename(fullPath));
  const lastMod = new Date(stat.mtimeMs).toUTCString();

  if (isDir) {
    return `  <D:response>
    <D:href>${href}</D:href>
    <D:propstat>
      <D:prop>
        <D:displayname>${escapeXml(displayName)}</D:displayname>
        <D:resourcetype><D:collection/></D:resourcetype>
        <D:getlastmodified>${lastMod}</D:getlastmodified>
      </D:prop>
      <D:status>HTTP/1.1 200 OK</D:status>
    </D:propstat>
  </D:response>`;
  }

  const mime = getMimeType(fullPath);
  return `  <D:response>
    <D:href>${href}</D:href>
    <D:propstat>
      <D:prop>
        <D:displayname>${escapeXml(displayName)}</D:displayname>
        <D:resourcetype/>
        <D:getcontentlength>${stat.size}</D:getcontentlength>
        <D:getcontenttype>${escapeXml(mime)}</D:getcontenttype>
        <D:getlastmodified>${lastMod}</D:getlastmodified>
      </D:prop>
      <D:status>HTTP/1.1 200 OK</D:status>
    </D:propstat>
  </D:response>`;
}

function extractDavSubpath(rawUrl: string): string {
  const urlObj = new URL(rawUrl, 'http://localhost');
  let pathname = decodeURIComponent(urlObj.pathname);
  if (pathname === '/dav' || pathname === '/dav/') {
    return '/';
  }
  if (pathname.startsWith('/dav/')) {
    pathname = pathname.slice(4);
  }
  return pathname || '/';
}

export async function handleWebDavRequest(
  req: IncomingMessage,
  res: ServerResponse
): Promise<void> {
  const method = (req.method || 'GET').toUpperCase();
  const davHeaders: Record<string, string> = {
    DAV: '1, 2',
    'MS-Author-Via': 'DAV',
    Allow: 'OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE, PROPFIND, PROPPATCH, LOCK, UNLOCK',
    'Accept-Ranges': 'bytes',
  };

  for (const [k, v] of Object.entries(davHeaders)) {
    res.setHeader(k, v);
  }

  if (method === 'OPTIONS') {
    res.statusCode = 200;
    res.end();
    return;
  }

  const access = engine.canAccessPhoneVfsWithoutTouch();
  if (!access.ok) {
    res.statusCode = 503;
    res.setHeader('Content-Type', 'text/plain; charset=utf-8');
    res.end(`ChainIt Z:\\ Sibling VFS unavailable: ${access.reason}`);
    return;
  }

  let resolved: ReturnType<typeof resolveSafePath>;
  try {
    const subPath = extractDavSubpath(req.url || '/dav/');
    resolved = resolveSafePath('phone', subPath);
  } catch (err) {
    res.statusCode = 403;
    res.end(err instanceof Error ? err.message : 'Forbidden');
    return;
  }

  try {
    if (method === 'PROPFIND') {
      if (!fs.existsSync(resolved.fullPath)) {
        res.statusCode = 404;
        res.end('Not Found');
        return;
      }
      const depthHeader = String(req.headers['depth'] ?? '1').trim();
      const responses: string[] = [buildPropResponseXml(resolved.virtualPath, resolved.fullPath)];

      const stat = fs.statSync(resolved.fullPath);
      if (stat.isDirectory() && depthHeader !== '0') {
        for (const child of fs.readdirSync(resolved.fullPath)) {
          if (child.startsWith('.')) continue;
          const childFull = path.join(resolved.fullPath, child);
          const childVirtual =
            resolved.virtualPath === '/' ? `/${child}` : `${resolved.virtualPath}/${child}`;
          responses.push(buildPropResponseXml(childVirtual, childFull));
        }
      }

      const xml = `<?xml version="1.0" encoding="utf-8"?>\n<D:multistatus xmlns:D="DAV:">\n${responses.join(
        '\n'
      )}\n</D:multistatus>`;

      engine.addLog(
        'WEBDAV',
        `PROPFIND Z:${resolved.virtualPath.replace(/\//g, '\\')} (Depth: ${depthHeader})`,
        `Returned ${responses.length} VFS entries over ${engine.activeTransportId} (screen-off safe).`
      );
      engine.broadcast('snapshot');

      res.statusCode = 207;
      res.setHeader('Content-Type', 'application/xml; charset="utf-8"');
      res.end(xml);
      return;
    }

    if (method === 'GET' || method === 'HEAD') {
      if (!fs.existsSync(resolved.fullPath)) {
        res.statusCode = 404;
        res.end('Not Found');
        return;
      }
      const stat = fs.statSync(resolved.fullPath);
      if (stat.isDirectory()) {
        res.statusCode = 200;
        res.setHeader('Content-Type', 'text/plain; charset=utf-8');
        res.end(`ChainIt Z:\\ Directory: ${resolved.virtualPath}`);
        return;
      }

      res.setHeader('Content-Type', getMimeType(resolved.fullPath));
      const range = req.headers.range;
      if (range && range.startsWith('bytes=')) {
        const [startStr, endStr] = range.replace('bytes=', '').split('-');
        const start = parseInt(startStr, 10) || 0;
        const end = endStr ? Math.min(parseInt(endStr, 10), stat.size - 1) : stat.size - 1;
        res.statusCode = 206;
        res.setHeader('Content-Range', `bytes ${start}-${end}/${stat.size}`);
        res.setHeader('Content-Length', String(end - start + 1));
        if (method === 'HEAD') {
          res.end();
          return;
        }
        fs.createReadStream(resolved.fullPath, { start, end }).pipe(res);
        return;
      }

      res.statusCode = 200;
      res.setHeader('Content-Length', String(stat.size));
      if (method === 'HEAD') {
        res.end();
        return;
      }
      engine.addLog(
        'WEBDAV',
        `GET Z:${resolved.virtualPath.replace(/\//g, '\\')} (${stat.size} B)`,
        `Streamed directly from Sibling storage.`
      );
      fs.createReadStream(resolved.fullPath).pipe(res);
      return;
    }

    if (method === 'PUT') {
      const existed = fs.existsSync(resolved.fullPath);
      fs.mkdirSync(path.dirname(resolved.fullPath), { recursive: true });
      const writeStream = fs.createWriteStream(resolved.fullPath);
      let written = 0;
      await new Promise<void>((resolve, reject) => {
        req.on('data', (chunk: Buffer) => {
          written += chunk.length;
        });
        req.pipe(writeStream);
        writeStream.on('finish', () => resolve());
        writeStream.on('error', reject);
      });

      engine.recomputeState(written);
      engine.addLog(
        'WEBDAV',
        `PUT Z:${resolved.virtualPath.replace(/\//g, '\\')} (${written} B)`,
        `Written directly to Android storage via Z:\\ WebDAV.`
      );
      engine.broadcast('vfs_changed', { targets: ['phone'] });
      res.statusCode = existed ? 204 : 201;
      res.end();
      return;
    }

    if (method === 'MKCOL') {
      if (fs.existsSync(resolved.fullPath)) {
        res.statusCode = 405;
        res.end('Already exists');
        return;
      }
      fs.mkdirSync(resolved.fullPath, { recursive: true });
      engine.addLog('WEBDAV', `MKCOL Z:${resolved.virtualPath.replace(/\//g, '\\')}`, 'Created folder on Z:\\');
      engine.broadcast('vfs_changed', { targets: ['phone'] });
      res.statusCode = 201;
      res.end();
      return;
    }

    if (method === 'DELETE') {
      if (!fs.existsSync(resolved.fullPath)) {
        res.statusCode = 404;
        res.end('Not Found');
        return;
      }
      fs.rmSync(resolved.fullPath, { recursive: true, force: true });
      engine.addLog('WEBDAV', `DELETE Z:${resolved.virtualPath.replace(/\//g, '\\')}`, 'Removed item from Z:\\');
      engine.broadcast('vfs_changed', { targets: ['phone'] });
      res.statusCode = 204;
      res.end();
      return;
    }

    if (method === 'MOVE' || method === 'COPY') {
      const destHeader = req.headers['destination'];
      if (!destHeader || typeof destHeader !== 'string') {
        res.statusCode = 400;
        res.end('Missing Destination header');
        return;
      }
      const destSub = extractDavSubpath(destHeader);
      const destResolved = resolveSafePath('phone', destSub);
      fs.mkdirSync(path.dirname(destResolved.fullPath), { recursive: true });

      const existed = fs.existsSync(destResolved.fullPath);
      if (method === 'COPY') {
        fs.cpSync(resolved.fullPath, destResolved.fullPath, { recursive: true, force: true });
      } else {
        if (existed) {
          fs.rmSync(destResolved.fullPath, { recursive: true, force: true });
        }
        fs.renameSync(resolved.fullPath, destResolved.fullPath);
      }

      engine.addLog(
        'WEBDAV',
        `${method} Z:${resolved.virtualPath} -> Z:${destResolved.virtualPath}`,
        'Executed atomic VFS operation on Z:\\'
      );
      engine.broadcast('vfs_changed', { targets: ['phone'] });
      res.statusCode = existed ? 204 : 201;
      res.end();
      return;
    }

    // Windows Explorer WebClient sends LOCK / UNLOCK / PROPPATCH when editing files in place
    if (method === 'LOCK') {
      const token = 'opaquelocktoken:chainit-sibling-lock-0001';
      res.statusCode = 200;
      res.setHeader('Lock-Token', `<${token}>`);
      res.setHeader('Content-Type', 'application/xml; charset="utf-8"');
      res.end(`<?xml version="1.0" encoding="utf-8"?>
<D:prop xmlns:D="DAV:">
  <D:lockdiscovery>
    <D:activelock>
      <D:locktype><D:write/></D:locktype>
      <D:lockscope><D:exclusive/></D:lockscope>
      <D:depth>infinity</D:depth>
      <D:timeout>Second-3600</D:timeout>
      <D:locktoken><D:href>${token}</D:href></D:locktoken>
    </D:activelock>
  </D:lockdiscovery>
</D:prop>`);
      return;
    }

    if (method === 'UNLOCK') {
      res.statusCode = 204;
      res.end();
      return;
    }

    if (method === 'PROPPATCH') {
      res.statusCode = 207;
      res.setHeader('Content-Type', 'application/xml; charset="utf-8"');
      res.end(`<?xml version="1.0" encoding="utf-8"?>
<D:multistatus xmlns:D="DAV:">
  <D:response>
    <D:href>${encodeDavHref(resolved.virtualPath, false)}</D:href>
    <D:propstat>
      <D:prop/>
      <D:status>HTTP/1.1 200 OK</D:status>
    </D:propstat>
  </D:response>
</D:multistatus>`);
      return;
    }

    res.statusCode = 405;
    res.end(`Unsupported WebDAV method: ${method}`);
  } catch (err) {
    res.statusCode = 500;
    res.end(err instanceof Error ? err.message : 'Internal WebDAV Error');
  }
}
