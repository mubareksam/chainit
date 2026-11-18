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
    // The rate has to be *measured*: derivable from the bytes and duration the same entry
    // reports. A nominal transport speed (the old `link.speedMBps || 85`) passes every other
    // assertion while telling the user something that never happened.
    assert.ok(entry.durationMs >= 1, 'the entry carries a real elapsed time');
    const impliedMBps = entry.bytes / (1024 * 1024) / (entry.durationMs / 1000);
    assert.ok(
      Math.abs(entry.effectiveSpeedMBps - impliedMBps) <= Math.max(0.05, impliedMBps * 0.25),
      `effectiveSpeedMBps ${entry.effectiveSpeedMBps} is not what ${entry.bytes} B in ` +
        `${entry.durationMs} ms implies (${impliedMBps.toFixed(2)})`
    );
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
    assert.ok(pulled.durationMs >= 1, 'the saved file carries a real elapsed time, not 0');
    assert.ok(
      pulled.effectiveSpeedMBps > 0,
      'and a measured rate - a 0.0 MB/s entry renders in the Recent tab with no speed at all'
    );
    const pulledImplied = pulled.bytes / (1024 * 1024) / (pulled.durationMs / 1000);
    assert.ok(
      Math.abs(pulled.effectiveSpeedMBps - pulledImplied) <= Math.max(0.05, pulledImplied * 0.25),
      `saved-file rate ${pulled.effectiveSpeedMBps} does not match ${pulled.bytes} B in ${pulled.durationMs} ms`
    );

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

  // ---------------------------------------------------------------------------------------
  // 8. Bluetooth RFCOMM: the dial path must be fast, recoverable and honest.
  //
  //    "Improve the bluetooth connectivity, it still didn't connect" is not fixed by a nicer
  //    status line. These assertions pin the behaviour that was broken:
  //      a. a warm link performs NO further SDP query (the cache is read, not just displayed);
  //      b. a handset that is listening but published no SDP record is rescued by a verified
  //         channel sweep instead of being reported unreachable;
  //      c. the phone's own address+channel, learned from a beacon, is dialled directly;
  //      d. a closed listener fails with a stated cause, and the snapshot then reports NO
  //         active link - never a link that looks connected and serves nothing.
  // ---------------------------------------------------------------------------------------
  {
    const HANDSET = 'C4:A1:B1:70:00:0E';
    const probe = async (body: Record<string, unknown>) =>
      (
        await fetch(`${BASE}/api/bluetooth/probe`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body),
        })
      ).json();
    const env = (patch: Record<string, unknown>) =>
      fetch(`${BASE}/api/environment/update`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch),
      });

    await env({ btListenerOpen: true, btSdpVisible: true, btRfcommChannel: 7 });

    // Counters are read as *deltas*: this process may already have dialled (the preview is long
    // lived, and /api/state is polled), so absolute totals would make the suite depend on being
    // the first thing to touch the engine. A delta is both robust and the stricter claim - it
    // says what THIS dial cost, not what the process has cost so far.
    const baseline = (await (await fetch(`${BASE}/api/state`)).json()).bluetooth;

    // (a) a cold dial has to resolve the channel: SDP answers, and the channel is remembered.
    const cold = await probe({ endpoint: `bt://${HANDSET}`, forgetCache: true });
    assert.equal(cold.dial.ok, true, 'the first Bluetooth dial reaches the phone');
    assert.equal(cold.dial.channel, 7, 'and it lands on the phone\'s real RFCOMM channel');
    assert.equal(cold.dial.strategy, 'sdp', 'a cold dial resolves through SDP');
    assert.equal(cold.bluetooth.sdpQueries, baseline.sdpQueries + 1, 'it cost exactly one SDP query');
    assert.equal(cold.bluetooth.cachedRfcommChannel, 7, 'the resolved channel is cached');
    assert.equal(cold.bluetooth.channelCacheEntries, 1, 'for this one handset');

    // The whole point of the cache: the next dial must not touch SDP again.
    const warm = await probe({ endpoint: `bt://${HANDSET}` });
    assert.equal(warm.dial.ok, true, 'the second dial succeeds too');
    assert.equal(warm.dial.strategy, 'cached', 'and it reuses the remembered channel');
    assert.equal(warm.bluetooth.sdpQueries, cold.bluetooth.sdpQueries, 'SDP was NOT queried a second time');
    assert.equal(warm.bluetooth.channelCacheHits, cold.bluetooth.channelCacheHits + 1, 'the cache hit is counted');
    assert.ok(
      Number.isFinite(warm.bluetooth.requestsServed) && Number.isFinite(warm.bluetooth.dials),
      'served-request and dial telemetry are real numbers, not invented constants'
    );

    // A stale channel must not be retried forever: it is forgotten and re-resolved.
    await env({ btRfcommChannel: 9 });
    const moved = await probe({ endpoint: `bt://${HANDSET}` });
    assert.equal(moved.dial.ok, true, 'a handset whose listener moved is still reached');
    assert.equal(moved.dial.channel, 9, 'on its new channel');
    assert.equal(moved.bluetooth.cachedRfcommChannel, 9, 'and the cache follows it');

    // (b) listening but invisible to SDP - the failure that used to leave the link dead.
    // Changing the radio conditions under a pinned link makes the engine re-dial (that is the
    // correct behaviour, and it is what the native discovery loop does), so the rescue counter is
    // compared as a delta: this dial must add exactly one, whatever the previous total was.
    await env({ btSdpVisible: false, btRfcommChannel: 5 });
    // Changing the radio under a pinned link re-dials it immediately, and with SDP now invisible
    // that re-dial has to be rescued by the sweep on its own. Assert that too: a link that only
    // recovers on the next manual probe is not a link that "works smoothly".
    const afterRadioChange = (await (await fetch(`${BASE}/api/state`)).json()).bluetooth;
    assert.ok(
      afterRadioChange.guessRecoveredLinks > moved.bluetooth.guessRecoveredLinks,
      'the engine re-dials on its own when the radio conditions change under a live link'
    );
    const rescuesBefore = afterRadioChange.guessRecoveredLinks;
    const swept = await probe({ endpoint: `bt://${HANDSET}`, forgetCache: true });
    assert.equal(swept.dial.ok, true, 'a phone with no SDP record is still reachable');
    assert.equal(swept.dial.strategy, 'guess', 'by the verified channel sweep');
    assert.equal(swept.dial.channel, 5, 'which finds the channel the listener is really on');
    assert.equal(
      swept.bluetooth.guessRecoveredLinks,
      rescuesBefore + 1,
      'and this dial is counted as exactly one rescue'
    );

    // (c) the phone reports itself, so the PC dials the channel with no SDP and no sweep.
    const beforeQueries = swept.bluetooth.sdpQueries;
    const learned = await (
      await fetch(`${BASE}/api/bluetooth/learn`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ btAddress: 'c4-a1-b1-70-00-0e', btRfcommChannel: 5, btListening: true }),
      })
    ).json();
    assert.equal(
      learned.learnedEndpoint,
      `bt://${HANDSET}:5`,
      'the beacon\'s address and channel become a directly dialable endpoint'
    );
    const direct = await probe({ endpoint: `bt://${HANDSET}:5` });
    assert.equal(direct.dial.ok, true, 'the explicit channel connects');
    assert.equal(direct.dial.strategy, 'explicit', 'with no resolution step at all');
    assert.equal(direct.bluetooth.sdpQueries, beforeQueries, 'and no extra SDP query');

    // (d) a closed listener must fail loudly and must NOT be reported as an active link.
    await env({ btListenerOpen: false });
    const dead = await probe({ endpoint: `bt://${HANDSET}` });
    assert.equal(dead.dial.ok, false, 'a closed listener does not connect');
    assert.ok(dead.dial.error.length > 10, 'and the failure states a cause');
    assert.ok(
      dead.dial.error.toLowerCase().includes('listener') ||
        dead.dial.error.toLowerCase().includes('refused'),
      `the cause is specific, not generic (got: ${dead.dial.error})`
    );
    assert.equal(dead.bluetooth.lastDialError, dead.dial.error, 'the snapshot carries the same cause');
    assert.equal(dead.bluetooth.activeLink, '', 'no link is claimed while the dial fails');
    assert.equal(dead.bluetooth.pooledChannels, 0, 'and no channel is parked for it');

    // A manual connect must refuse rather than pretend.
    const refused = await fetch(`${BASE}/api/transport/connect-ip`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ phoneIp: `bt://${HANDSET}` }),
    });
    assert.equal(refused.status, 400, 'connect-ip does not report a link that did not come up');
    const refusedBody = (await refused.json()) as { error?: string };
    assert.ok(refusedBody.error && refusedBody.error.length > 20, 'and it says why');

    // Restore a working radio for the rest of the suite.
    await env({ btListenerOpen: true, btSdpVisible: true, btRfcommChannel: 7 });
    const restored = await probe({ endpoint: `bt://${HANDSET}`, forgetCache: true });
    assert.equal(restored.dial.ok, true, 'the link comes back once the listener reopens');
    assert.equal(restored.bluetooth.lastDialError, '', 'and the failure is cleared, not sticky');
  }
  console.log('✓ Bluetooth RFCOMM dial policy verified (cache-first, SDP fallback, verified sweep, honest failure)');

  // ---------------------------------------------------------------------------------------
  // 9. Bluetooth parity: the offline link must carry the same *information* as Wi-Fi, and must
  //    never claim a transfer it did not make.
  //
  //    "It must work same smooth fast and functionality as the system works under wifi, like it
  //    must show the battery" - so these assertions are about what the link tells the user, not
  //    only about bytes moving. On the native engine the battery reading used to have exactly one
  //    writer (the IP ping), so a Bluetooth-only session had no battery at all, and a failed
  //    phone-targeted transfer was reported as `completed` with a measured speed.
  //      a. with Bluetooth as the only link, telemetry is live and names Bluetooth as its carrier;
  //      b. the percentage is the phone's own, never a default the engine filled in;
  //      c. a transfer to an unreachable phone is refused and records no history row;
  //      d. the phone pane is not served from a folder on this PC wearing the phone's name.
  // ---------------------------------------------------------------------------------------
  {
    const HANDSET = 'C4:A1:B1:70:00:0E';
    const env = (patch: Record<string, unknown>) =>
      fetch(`${BASE}/api/environment/update`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch),
      });
    const state = async () => (await fetch(`${BASE}/api/state`)).json();
    const historyCount = async () =>
      ((await (await fetch(`${BASE}/api/history?limit=60`)).json()).items || []).length;

    // (a) Bluetooth alone: no Wi-Fi, no hotspot, no USB, no relay.
    await env({
      usbPlugged: false,
      sharedWifiAvailable: false,
      localHotspotActive: false,
      onlineP2pEnabled: false,
      bluetoothEnabled: true,
      btListenerOpen: true,
      btSdpVisible: true,
      btRfcommChannel: 7,
      phoneBatteryPct: 41,
    });
    const linked = await fetch(`${BASE}/api/transport/connect-ip`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ phoneIp: `bt://${HANDSET}` }),
    });
    assert.equal(linked.status, 200, 'a Bluetooth-only link is accepted with no other transport up');

    const btOnly = await state();
    assert.equal(btOnly.activeTransportId, 'bluetooth', 'Bluetooth is the active transport');
    assert.equal(btOnly.environment.phoneTelemetryLive, true, 'and its telemetry is live');
    assert.equal(btOnly.environment.phoneBatteryKnown, true, 'a real percentage was reported');
    assert.equal(
      btOnly.environment.phoneTelemetryTransport,
      'bluetooth',
      'the reading names the link that carried it - the offline one, not a stale Wi-Fi report'
    );

    // (b) 41 was reported, so 41 must come back. The engine's old default was a literal 88 and the
    // preview's is 86; either one showing up here would mean a number nobody measured.
    assert.equal(btOnly.environment.phoneBatteryPct, 41, "the percentage is the phone's own");

    // (c) take the radio away: a phone-targeted transfer must be refused, not reported complete.
    await env({ bluetoothEnabled: false, btListenerOpen: false });
    const before = await historyCount();
    const refused = await fetch(`${BASE}/api/vfs/transfer`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        op: 'copy',
        fromTarget: 'pc',
        fromPaths: ['/Documents/Sprint_Tasks.txt'],
        toTarget: 'phone',
        toDir: '/Download',
      }),
    });
    assert.notEqual(refused.status, 200, 'a transfer to an unreachable phone is not a success');
    const refusedBody = await refused.json();
    assert.ok(
      typeof refusedBody.error === 'string' && refusedBody.error.length > 10,
      'and it states why, instead of quietly doing something local'
    );
    assert.equal(
      await historyCount(),
      before,
      'nothing that did not move was recorded as having moved'
    );

    // (d) the phone pane must not be a folder on this PC wearing the phone's name.
    const pane = await fetch(`${BASE}/api/vfs/phone/list?path=/`);
    assert.equal(pane.status, 503, 'listing the phone with no phone linked is refused');
    const paneBody = await pane.json();
    assert.ok(
      typeof paneBody.error === 'string' && paneBody.error.length > 10,
      'with a reason a user can act on'
    );

    // Leave the preview the way the rest of the suite expects it.
    await env({
      bluetoothEnabled: true,
      btListenerOpen: true,
      btSdpVisible: true,
      sharedWifiAvailable: true,
      phoneBatteryPct: 86,
    });
  }
  console.log(
    '✓ Bluetooth parity verified (offline telemetry carrier, no invented percentage, honest refusal)'
  );

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
