import assert from 'node:assert/strict';

const BASE = `http://127.0.0.1:${process.env.PORT || 4173}`;

async function run() {
  console.log(`Running ChainIt end-to-end verification against ${BASE} ...`);

  // 1. UI endpoints
  const htmlRes = await fetch(`${BASE}/`);
  assert.equal(htmlRes.status, 200);
  const html = await htmlRes.text();
  assert.ok(html.includes('<div id="root"></div>'), 'Desktop HTML served');

  // 2. Initial Sibling & Screen-Off Z:\ State
  const state1 = await (await fetch(`${BASE}/api/state`)).json();
  assert.equal(state1.sibling.enabled, true);
  assert.equal(state1.environment.phoneScreenLocked, true);
  assert.equal(state1.sibling.zDriveMounted, true);
  assert.equal(state1.activeTransportId, 'wifi_lan');
  console.log('✓ Initial Sibling state & screen-off Z:\\ mount verified');

  // 3. Browse phone storage while screen is OFF
  const camRes = await (await fetch(`${BASE}/api/vfs/phone/list?path=/DCIM/Camera`)).json();
  assert.ok(Array.isArray(camRes.items) && camRes.items.length >= 2);
  console.log(`✓ Browsed Z:\\DCIM\\Camera with phone screen OFF (${camRes.items.length} files)`);

  // 4. WebDAV RFC 4918 OPTIONS + PROPFIND + PUT + GET + DELETE
  const optRes = await fetch(`${BASE}/dav/`, { method: 'OPTIONS' });
  assert.equal(optRes.status, 200);
  assert.equal(optRes.headers.get('ms-author-via'), 'DAV');

  const propRes = await fetch(`${BASE}/dav/`, {
    method: 'PROPFIND',
    headers: { Depth: '1' },
  });
  assert.equal(propRes.status, 207);
  const xml = await propRes.text();
  assert.ok(xml.includes('/dav/DCIM/'), 'PROPFIND includes DCIM collection');

  const putPayload = 'Verified Windows Z:\\ WebDAV roundtrip write & read!';
  const putRes = await fetch(`${BASE}/dav/Download/selftest_probe.txt`, {
    method: 'PUT',
    body: putPayload,
  });
  assert.ok(putRes.status === 201 || putRes.status === 204);

  const getRes = await fetch(`${BASE}/dav/Download/selftest_probe.txt`);
  assert.equal(getRes.status, 200);
  assert.equal(await getRes.text(), putPayload);

  const delRes = await fetch(`${BASE}/dav/Download/selftest_probe.txt`, { method: 'DELETE' });
  assert.equal(delRes.status, 204);
  console.log('✓ RFC 4918 WebDAV Z:\\ server (OPTIONS, PROPFIND, PUT, GET, DELETE) verified');

  // 5. Smart Transport Auto-Selection (USB priority & Routerless BLE -> 5 GHz Hotspot promotion)
  const usbState = await (
    await fetch(`${BASE}/api/environment/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ usbPlugged: true }),
    })
  ).json();
  assert.equal(usbState.activeTransportId, 'usb_rndis');

  const offlineBleState = await (
    await fetch(`${BASE}/api/environment/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        usbPlugged: false,
        sharedWifiAvailable: false,
        localHotspotActive: false,
        bluetoothEnabled: true,
      }),
    })
  ).json();
  assert.equal(offlineBleState.activeTransportId, 'bluetooth');

  const transferBoost = await (
    await fetch(`${BASE}/api/vfs/transfer`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        op: 'copy',
        fromTarget: 'pc',
        fromPaths: ['/Desktop/Sprint_Tasks.txt'],
        toTarget: 'phone',
        toDir: '/ChainIt_Inbox',
      }),
    })
  ).json();
  assert.equal(transferBoost.status, 'completed');
  assert.equal(transferBoost.snapshot.activeTransportId, 'local_hotspot');
  console.log('✓ Smart Transport selector (USB priority + BLE -> 5GHz Hotspot Boost) verified');

  // 6. Native PC file browser contract.
  //    The phone's PcBrowserActivity + PcVfsClient drive the PC through exactly these calls instead
  //    of opening http://<pc>:4173/mobile in a browser, so the shape is asserted here: if one of
  //    them changes, the phone's native browser breaks silently on a device nobody can debug from.
  const pcRoot = await (await fetch(`${BASE}/api/vfs/pc/list?path=/`)).json();
  assert.equal(pcRoot.currentPath, '/');
  assert.ok(Array.isArray(pcRoot.items) && pcRoot.items.length > 0, 'PC root lists items');
  for (const item of pcRoot.items) {
    for (const field of ['name', 'path', 'kind', 'sizeBytes', 'modifiedMs', 'mimeType', 'ext']) {
      assert.ok(field in item, `PC listing item carries ${field}`);
    }
    assert.ok(String(item.path).startsWith('/'), 'virtual paths stay inside the shared root');
    assert.ok(Number.isFinite(item.modifiedMs), 'modifiedMs is a number');
    assert.ok(item.modifiedMs <= Date.now() + 2000, 'modifiedMs is a real timestamp, never in the future');
  }
  const kinds = pcRoot.items.map((i: { kind: string }) => i.kind);
  const firstFile = kinds.indexOf('file');
  const lastDir = kinds.lastIndexOf('directory');
  assert.ok(firstFile === -1 || lastDir < firstFile, 'folders are listed before files');

  const probeDir = '/selftest-native-browser';
  const probeBody = 'sent from the phone side of the contract';
  const mkRes = await fetch(`${BASE}/api/vfs/pc/mkdir`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ parentDir: '/', name: 'selftest-native-browser' }),
  });
  assert.equal(mkRes.status, 200);

  const upRes = await fetch(
    `${BASE}/api/vfs/pc/upload?dir=${encodeURIComponent(probeDir)}&filename=probe.txt`,
    { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: probeBody },
  );
  assert.equal(upRes.status, 200);

  const probeListing = await (await fetch(`${BASE}/api/vfs/pc/list?path=${encodeURIComponent(probeDir)}`)).json();
  assert.equal(probeListing.currentPath, probeDir);
  assert.equal(probeListing.items.length, 1);
  const probe = probeListing.items[0];
  assert.equal(probe.name, 'probe.txt');
  assert.equal(probe.kind, 'file');
  assert.equal(probe.sizeBytes, Buffer.byteLength(probeBody));
  assert.ok(Math.abs(probe.modifiedMs - Date.now()) < 60_000, 'the new file reports its own modified time');

  const rawRes = await fetch(`${BASE}/api/vfs/pc/raw?path=${encodeURIComponent(probe.path)}&download=1`);
  assert.equal(rawRes.status, 200);
  assert.ok((rawRes.headers.get('content-disposition') || '').includes('attachment'), 'download=1 sets Content-Disposition');
  assert.equal(await rawRes.text(), probeBody);

  const textRes = await (await fetch(`${BASE}/api/vfs/pc/text?path=${encodeURIComponent(probe.path)}`)).json();
  assert.equal(typeof textRes.editable, 'boolean');
  assert.equal(textRes.content, probeBody);

  const renameRes = await fetch(`${BASE}/api/vfs/pc/rename`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ path: probe.path, newName: 'probe-renamed.txt' }),
  });
  assert.equal(renameRes.status, 200);

  // A file that is gone must be a 404 the phone can turn into a sentence, never an empty list.
  const goneRes = await fetch(`${BASE}/api/vfs/pc/raw?path=${encodeURIComponent(probeDir + '/probe.txt')}&download=1`);
  assert.equal(goneRes.status, 404);

  const deleteRes = await fetch(`${BASE}/api/vfs/pc/delete`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ paths: [probeDir] }),
  });
  assert.equal(deleteRes.status, 200);
  const afterDelete = await (await fetch(`${BASE}/api/vfs/pc/list?path=${encodeURIComponent(probeDir)}`)).json();
  assert.notEqual(afterDelete.currentPath, probeDir, 'a deleted folder is no longer served');
  console.log('✓ Native PC file browser API contract (list, mkdir, upload, raw, text, rename, delete, honest 404)');

  // Clean up test copy & restore clean default preview state
  await fetch(`${BASE}/api/vfs/phone/delete`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ paths: ['/ChainIt_Inbox/Sprint_Tasks.txt'] }),
  });

  await fetch(`${BASE}/api/environment/update`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      usbPlugged: false,
      sharedWifiAvailable: true,
      localHotspotActive: false,
      bluetoothEnabled: true,
      onlineP2pEnabled: false,
      phoneScreenLocked: true,
    }),
  });

  console.log('ALL CHAINIT TESTS PASSED!');
}

run().catch((err) => {
  console.error(err);
  process.exit(1);
});
