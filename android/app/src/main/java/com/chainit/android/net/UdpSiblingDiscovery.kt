package com.chainit.android.net

import android.app.KeyguardManager
import android.content.Context
import android.os.Build
import android.os.PowerManager
import org.json.JSONArray
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.util.Collections
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

data class LocalLinkEndpoint(
    val ifaceName: String,
    val ip: String,
    val broadcastIp: String?,
    val transportKind: String
)

data class DiscoveredPcInfo(
    val pcName: String,
    val ip: String,
    val httpPort: Int,
    val lastSeenMs: Long
)

class UdpSiblingDiscovery(
    private val context: Context,
    private val vfsPort: Int = 49180,
    private val discoveryPort: Int = 49179
) {
    @Volatile
    private var running = false
    private var socket: DatagramSocket? = null

    @Volatile
    var lastDiscoveredPc: DiscoveredPcInfo? = null
        private set

    fun getSiblingKey(): String {
        val prefs = context.getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        return prefs.getString("sibling_key", "CHAINIT-SIBLING") ?: "CHAINIT-SIBLING"
    }

    fun isSiblingEnabled(): Boolean {
        val prefs = context.getSharedPreferences("chainit_prefs", Context.MODE_PRIVATE)
        return prefs.getBoolean("sibling_enabled", true)
    }

    fun computeAuthToken(key: String, epochWindow: Long): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        val raw = mac.doFinal("CHAINIT:$epochWindow".toByteArray(Charsets.UTF_8))
        return raw.take(8).joinToString("") { "%02X".format(it) }
    }

    fun enumerateEndpoints(): List<LocalLinkEndpoint> {
        val result = mutableListOf<LocalLinkEndpoint>()
        try {
            val ifaces = Collections.list(NetworkInterface.getNetworkInterfaces())
            for (nif in ifaces) {
                if (!nif.isUp || nif.isLoopback) continue
                val name = nif.name.lowercase()
                val kind = when {
                    name.startsWith("rndis") || name.startsWith("usb") || name.startsWith("ncm") -> "usb_rndis"
                    name.startsWith("ap") || name.startsWith("swlan") || name.contains("p2p") || name.contains("softap") -> "local_hotspot"
                    name.startsWith("bt-pan") || name.startsWith("bnep") -> "bluetooth"
                    name.startsWith("wlan") || name.startsWith("eth") -> "wifi_lan"
                    else -> continue
                }
                for (ia in nif.interfaceAddresses) {
                    val addr = ia.address
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        val ip = addr.hostAddress ?: continue
                        // Heuristic: Android SoftAP often uses 192.168.43.1 or 192.168.49.1 on wlan1
                        val refinedKind = if (kind == "wifi_lan" && (ip.startsWith("192.168.43.") || ip.startsWith("192.168.49."))) {
                            "local_hotspot"
                        } else {
                            kind
                        }
                        result.add(
                            LocalLinkEndpoint(
                                ifaceName = nif.name,
                                ip = ip,
                                broadcastIp = ia.broadcast?.hostAddress,
                                transportKind = refinedKind
                            )
                        )
                    }
                }
            }
        } catch (_: Exception) {
        }
        return result
    }

    fun buildBeaconPayload(): ByteArray {
        val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        val km = context.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        val screenLocked = !pm.isInteractive || km.isKeyguardLocked
        val batt = BatteryTelemetry.read(context)
        val battery = batt.pct
        val epoch = System.currentTimeMillis() / 900_000L
        val key = getSiblingKey()

        val endpointsArr = JSONArray()
        for (ep in enumerateEndpoints()) {
            val eObj = JSONObject()
            eObj.put("iface", ep.ifaceName)
            eObj.put("ip", ep.ip)
            eObj.put("kind", ep.transportKind)
            endpointsArr.put(eObj)
        }

        val json = JSONObject()
        json.put("magic", "CHAINIT_SIBLING_ANNOUNCE_V1")
        json.put("role", "android_phone")
        json.put("deviceName", "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}")
        json.put("model", Build.MODEL)
        json.put("androidVersion", "Android ${Build.VERSION.RELEASE}")
        json.put("vfsPort", vfsPort)
        json.put("batteryPct", battery)
        json.put("charging", batt.charging)
        json.put("chargeSource", batt.source)
        json.put("temperatureC", batt.tempC)
        json.put("screenLocked", screenLocked)
        json.put("siblingEnabled", isSiblingEnabled())
        json.put("authTag", computeAuthToken(key, epoch))
        json.put("endpoints", endpointsArr)
        return json.toString().toByteArray(Charsets.UTF_8)
    }

    fun start() {
        if (running) return
        running = true

        // Receiver thread
        Thread {
            try {
                val sock = DatagramSocket(null)
                sock.reuseAddress = true
                sock.broadcast = true
                sock.bind(InetSocketAddress("0.0.0.0", discoveryPort))
                socket = sock

                val buf = ByteArray(4096)
                while (running) {
                    val pkt = DatagramPacket(buf, buf.size)
                    sock.receive(pkt)
                    val msg = String(pkt.data, 0, pkt.length, Charsets.UTF_8)
                    if (msg.contains("CHAINIT_DISCOVER_V1")) {
                        try {
                            val obj = JSONObject(msg)
                            val pcName = obj.optString("pcName", "Windows PC")
                            val pcHttpPort = obj.optInt("httpPort", 4173)
                            lastDiscoveredPc = DiscoveredPcInfo(
                                pcName = pcName,
                                ip = pkt.address.hostAddress ?: "",
                                httpPort = pcHttpPort,
                                lastSeenMs = System.currentTimeMillis()
                            )
                        } catch (_: Exception) {
                        }
                        if (isSiblingEnabled()) {
                            val reply = buildBeaconPayload()
                            val replyPkt = DatagramPacket(reply, reply.size, pkt.address, pkt.port)
                            sock.send(replyPkt)
                        }
                    }
                }
            } catch (_: Exception) {
            }
        }.apply { isDaemon = true; start() }

        // Periodic announce thread across all broadcast addresses
        Thread {
            while (running) {
                try {
                    if (isSiblingEnabled()) {
                        val s = socket
                        if (s != null && !s.isClosed) {
                            val payload = buildBeaconPayload()
                            val targets = mutableSetOf("255.255.255.255")
                            for (ep in enumerateEndpoints()) {
                                ep.broadcastIp?.let { targets.add(it) }
                            }
                            for (bcast in targets) {
                                try {
                                    val addr = InetAddress.getByName(bcast)
                                    s.send(DatagramPacket(payload, payload.size, addr, discoveryPort))
                                } catch (_: Exception) {
                                }
                            }
                        }
                    }
                    Thread.sleep(2500L)
                } catch (_: InterruptedException) {
                    break
                } catch (_: Exception) {
                }
            }
        }.apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
    }
}
