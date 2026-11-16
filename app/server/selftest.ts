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
