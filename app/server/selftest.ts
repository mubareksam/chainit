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

  // ---------------------------------------------------------------------------
  // 7. Volumes beyond C:, the reach switch, and the shared-file history (v1.7.0)
  // ---------------------------------------------------------------------------
  {
    const listRes = await (await fetch(`${BASE}/api/vfs/pc/list?path=/`)).json();
    assert.ok(Array.isArray(listRes.drives) && listRes.drives.length >= 1, 'a listing carries the volume list');
    const root = listRes.drives.find((d: { isPcRoot?: boolean }) => d.isPcRoot);
    assert.ok(root, 'exactly one volume is the profile root');
    assert.equal(root.virtualPath, '/', 'the profile root is still addressed as "/" so old clients work');
    assert.equal(typeof listRes.allowOtherDrives, 'boolean', 'the reach switch travels with every listing');

    const drivesRes = await fetch(`${BASE}/api/drives`);
    assert.equal(drivesRes.status, 200);
    const drives = await drivesRes.json();
    assert.ok(Array.isArray(drives.drives), '/api/drives lists volumes');
    assert.ok(drives.pcRootLetter, '/api/drives names the root volume letter');

    // A volume that is not there must be refused with the runtime's own sentence, never a bare 400
    // and never a silent jump back to the profile folder.
    const absent = await (await fetch(`${BASE}/api/vfs/pc/list?path=${encodeURIComponent('/Q:/nope')}`)).json();
    assert.deepEqual(absent.items, [], 'an absent volume lists nothing');
    assert.ok(typeof absent.error === 'string' && absent.error.length > 0, 'and says why');
    assert.ok(Array.isArray(absent.drives), 'the refusal still carries the volume list');

    // Narrowing the reach is a decision, so it is explicit, persisted and reported.
    const offRes = await fetch(`${BASE}/api/drives/scope`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ allowOtherDrives: false }),
    });
    assert.equal(offRes.status, 200);
    const off = await offRes.json();
    assert.equal(off.allowOtherDrives, false, 'the switch reports its new state');
    const blocked = await (await fetch(`${BASE}/api/vfs/pc/list?path=${encodeURIComponent('/Q:/nope')}`)).json();
    assert.ok(blocked.error && /switched off|not present/i.test(blocked.error), 'a blocked volume explains itself');
    await fetch(`${BASE}/api/drives/scope`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ allowOtherDrives: true }),
    });

    // The history records who moved a file and where it landed, so an entry is actionable.
    const upRes = await fetch(
      `${BASE}/api/vfs/pc/upload?dir=${encodeURIComponent('/Documents')}&filename=history-probe.txt`,
      { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: 'shared from the phone' }
    );
    assert.equal(upRes.status, 200);
    const histRes = await fetch(`${BASE}/api/history?limit=5`);
    assert.equal(histRes.status, 200);
    const hist = await histRes.json();
    assert.ok(Array.isArray(hist.items) && hist.items.length >= 1, '/api/history answers with entries');
    const entry = hist.items.find((t: { label?: string }) => t.label === 'history-probe.txt');
    assert.ok(entry, 'the upload the phone just pushed is in the history');
    assert.equal(entry.source, 'phone-upload', 'and it says the phone pushed it');
    assert.equal(entry.direction, 'phone_to_pc', 'with the direction it travelled');
    assert.ok(entry.virtualPath && entry.virtualPath.endsWith('/history-probe.txt'), 'and where it landed');
    assert.ok(hist.historyFile, 'the history file path is reported, not hidden');

    // An explicit save is a share; a preview is not.
    const saveRes = await fetch(
      `${BASE}/api/vfs/pc/raw?path=${encodeURIComponent('/Documents/history-probe.txt')}&download=1`
    );
    assert.equal(saveRes.status, 200);
    await new Promise((r) => setTimeout(r, 60));
    const afterSave = await (await fetch(`${BASE}/api/history?limit=5`)).json();
    const pulled = afterSave.items.find(
      (t: { label?: string; source?: string }) => t.label === 'history-probe.txt' && t.source === 'phone-download'
    );
    assert.ok(pulled, 'an explicit save (download=1) is recorded as the phone pulling the file');

    const revealRes = await fetch(`${BASE}/api/pc/reveal`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: '/Documents/history-probe.txt' }),
    });
    const reveal = await revealRes.json();
    assert.ok(reveal.localPath && reveal.localPath.length > 0, 'reveal resolves the virtual path to a real one');
    const revealGone = await fetch(`${BASE}/api/pc/reveal`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: '/Documents/never-existed.bin' }),
    });
    assert.equal(revealGone.status, 404, 'reveal refuses a path that is not there');

    const clearRes = await fetch(`${BASE}/api/history/clear`, { method: 'POST' });
    assert.equal(clearRes.status, 200);
    const cleared = await clearRes.json();
    assert.ok(cleared.removed >= 1, 'clearing reports how many entries went');
    const afterClear = await (await fetch(`${BASE}/api/history`)).json();
    assert.deepEqual(afterClear.items, [], 'and the history is then empty');

    // Battery detail travels with the percentage, and says whether it is live.
    const state = await (await fetch(`${BASE}/api/state`)).json();
    assert.equal(typeof state.environment.phoneBatteryPct, 'number', 'battery percentage is present');
    assert.equal(typeof state.environment.phoneCharging, 'boolean', 'charging state is present');
    assert.ok(state.environment.phoneChargeSource, 'the plug is named');
    assert.equal(typeof state.environment.phoneTemperatureC, 'number', 'temperature is present');
    assert.equal(typeof state.environment.phoneTelemetryLive, 'boolean', 'and whether the report is live');
    assert.ok(Array.isArray(state.pcVolumes), 'the snapshot carries the volume list');
    assert.equal(typeof state.allowOtherDrives, 'boolean', 'and the reach switch');

    // Leave the probe file out of the preview's seeded storage.
    await fetch(`${BASE}/api/vfs/pc/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ paths: ['/Documents/history-probe.txt'] }),
    });
  }
  console.log('✓ Volumes beyond C:, drive-reach switch, shared-file history and battery telemetry verified');

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
